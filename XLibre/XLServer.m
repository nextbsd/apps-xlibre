//
//  XLServer.m
//  XLibre.app - the Xnextbsd child process
//
//  Copyright (c) 2026, Joseph Maloney
//  SPDX-License-Identifier: MIT
//

#define _GNU_SOURCE 1   /* for environ on glibc */

#import "XLServer.h"

#import <errno.h>
#import <fcntl.h>
#import <poll.h>
#import <signal.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>
#import <sys/wait.h>
#import <unistd.h>

extern char **environ;

// The fd the server writes its display number to (-displayfd).
#define XL_DISPLAYFD 3
// How long to wait for the server to say which display it took.
#define XL_START_TIMEOUT_MS 30000

static BOOL reaped(pid_t pid);

volatile pid_t XLServerChildPid = 0;
static char serverAuthFile[1024];
static char serverAuthDir[1024];

void
XLServerEmergencyStop(void)
{
    if (XLServerChildPid > 0)
        kill(XLServerChildPid, SIGTERM);
    if (serverAuthFile[0] != '\0')
        unlink(serverAuthFile);
    if (serverAuthDir[0] != '\0')
        rmdir(serverAuthDir);
}

@implementation XLServer {
    NSString *_serverAuthDir;
    NSString *_serverAuth;
    NSString *_xauth;
    BOOL _addedCookie;
}

@synthesize display = _display;
@synthesize authority = _authority;
@synthesize pid = _pid;

#pragma mark - Paths

// Xnextbsd: the XLibreServerPath default, else Resources/Xnextbsd.
+ (NSString *)serverPath
{
    NSString *path = [[NSUserDefaults standardUserDefaults]
                         stringForKey:@"XLibreServerPath"];

    if ([path length] == 0)
        path = [[NSBundle mainBundle] pathForResource:@"Xnextbsd" ofType:nil];
    return path;
}

// xauth: the XLibreXauthPath default, else /usr/X11/bin/xauth or PATH.
+ (NSString *)xauthPath
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *path = [[NSUserDefaults standardUserDefaults]
                         stringForKey:@"XLibreXauthPath"];
    NSMutableArray *dirs = [NSMutableArray arrayWithObject:@"/usr/X11/bin"];
    NSString *env = [[[NSProcessInfo processInfo] environment]
                        objectForKey:@"PATH"];

    if ([path length] > 0)
        return path;
    if (env != nil)
        [dirs addObjectsFromArray:[env componentsSeparatedByString:@":"]];
    for (NSString *dir in dirs) {
        path = [dir stringByAppendingPathComponent:@"xauth"];
        if ([fm isExecutableFileAtPath:path])
            return path;
    }
    return nil;
}

// The Xauthority clients read: XAUTHORITY, else ~/.Xauthority.
+ (NSString *)userAuthority
{
    NSString *path = [[[NSProcessInfo processInfo] environment]
                         objectForKey:@"XAUTHORITY"];

    if ([path length] == 0)
        path = [NSHomeDirectory() stringByAppendingPathComponent:@".Xauthority"];
    return path;
}

#pragma mark - xauth

- (BOOL)xauth:(NSArray *)args
{
    NSTask *task = [[NSTask alloc] init];

    [task setLaunchPath:_xauth];
    [task setArguments:args];
    [task setStandardInput:[NSFileHandle fileHandleWithNullDevice]];
    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException *e) {
        NSLog(@"[XLibre] can't run %@: %@", _xauth, [e reason]);
        return NO;
    }
    if ([task terminationStatus] != 0) {
        NSLog(@"[XLibre] %@ %@ failed (%d)", _xauth,
              [args componentsJoinedByString:@" "], [task terminationStatus]);
        return NO;
    }
    return YES;
}

// 128 random bits, as 32 hex digits.
static NSString *
newCookie(void)
{
    unsigned char bytes[16];
    NSMutableString *hex = [NSMutableString string];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, bytes, sizeof(bytes)) : -1;
    unsigned i;

    if (fd >= 0)
        close(fd);
    if (n != (ssize_t)sizeof(bytes))
        return nil;
    for (i = 0; i < sizeof(bytes); i++)
        [hex appendFormat:@"%02x", bytes[i]];
    return hex;
}

#pragma mark - Starting

static char *
dupString(NSString *s)
{
    return strdup([s fileSystemRepresentation]);
}

// The server's environment: ours, without any stale XNEXTBSD_JOIN, plus
// the WindowServer credentials. The token never goes on argv, where ps
// would show it to other users.
static char **
serverEnvironment(NSDictionary *credentials)
{
    size_t n = 0, i, j = 0;
    char **envp;

    while (environ[n] != NULL)
        n++;
    envp = calloc(n + 2, sizeof(char *));
    if (envp == NULL)
        return NULL;
    for (i = 0; i < n; i++)
        if (strncmp(environ[i], "XNEXTBSD_JOIN=", 14) != 0)
            envp[j++] = environ[i];
    if (credentials != nil) {
        NSString *join = [NSString stringWithFormat:@"XNEXTBSD_JOIN=%u:%llu",
            [[credentials objectForKey:@"ClientID"] unsignedIntValue],
            [[credentials objectForKey:@"Token"] unsignedLongLongValue]];
        envp[j++] = strdup([join UTF8String]);
    }
    envp[j] = NULL;
    return envp;
}

- (BOOL)startWithCredentials:(NSDictionary *)credentials
                       error:(NSString **)error
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *server = [[self class] serverPath];
    NSString *cookie, *tmpl;
    NSMutableArray *args;
    char dirbuf[1024], **argv, **envp, buf[32];
    int fds[2];
    long maxfd;
    size_t len = 0, i;
    struct pollfd pfd;

    if ([server length] == 0 || ![fm isExecutableFileAtPath:server]) {
        *error = [NSString stringWithFormat:@"There is no X server at %@. "
                  "Install Xnextbsd into the app with "
                  "\"gmake install XNEXTBSD=/path/to/Xnextbsd\".",
                  server ?: @"XLibre.app/Resources/Xnextbsd"];
        return NO;
    }
    _xauth = [[self class] xauthPath];
    if (_xauth == nil) {
        *error = @"xauth isn't installed (looked in /usr/X11/bin and PATH).";
        return NO;
    }
    cookie = newCookie();
    if (cookie == nil) {
        *error = @"Can't read /dev/urandom for an X cookie.";
        return NO;
    }

    // The server reads every cookie in its -auth file whatever display it
    // names, so the file can be written before the display is known.
    tmpl = [NSTemporaryDirectory() stringByAppendingPathComponent:@"xlibre.XXXXXX"];
    snprintf(dirbuf, sizeof(dirbuf), "%s", [tmpl fileSystemRepresentation]);
    if (mkdtemp(dirbuf) == NULL) {
        *error = [NSString stringWithFormat:@"Can't make a directory in %@: %s",
                  NSTemporaryDirectory(), strerror(errno)];
        return NO;
    }
    _serverAuthDir = [fm stringWithFileSystemRepresentation:dirbuf
                                                     length:strlen(dirbuf)];
    _serverAuth = [_serverAuthDir stringByAppendingPathComponent:@"serverauth"];
    close(open([_serverAuth fileSystemRepresentation],
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    snprintf(serverAuthDir, sizeof(serverAuthDir), "%s", dirbuf);
    snprintf(serverAuthFile, sizeof(serverAuthFile), "%s",
             [_serverAuth fileSystemRepresentation]);
    if (![self xauth:@[@"-q", @"-f", _serverAuth, @"add", @":0", @".", cookie]]) {
        *error = @"xauth couldn't write the X server's cookie.";
        [self removeServerAuth];
        return NO;
    }

    args = [NSMutableArray arrayWithObjects:server,
            @"-displayfd", [NSString stringWithFormat:@"%d", XL_DISPLAYFD],
            @"-nolisten", @"tcp", @"-auth", _serverAuth, nil];
    // XLibreLowResolution: X draws at 1x on a HiDPI display, and
    // WindowServer scales it up.
    if ([[NSUserDefaults standardUserDefaults]
            boolForKey:@"XLibreLowResolution"])
        [args addObject:@"-lowres"];
    for (id a in [[NSUserDefaults standardUserDefaults]
                    arrayForKey:@"XLibreServerArguments"])
        if ([a isKindOfClass:[NSString class]])
            [args addObject:a];

    // Everything the child needs is made before fork(): after it, only
    // async-signal-safe calls.
    argv = calloc([args count] + 1, sizeof(char *));
    envp = serverEnvironment(credentials);
    if (argv == NULL || envp == NULL || pipe(fds) < 0) {
        *error = [NSString stringWithFormat:@"Can't start the X server: %s",
                  strerror(errno)];
        free(argv);
        free(envp);
        [self removeServerAuth];
        return NO;
    }
    for (i = 0; i < [args count]; i++)
        argv[i] = dupString([args objectAtIndex:i]);
    maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536)
        maxfd = 65536;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    _pid = fork();
    if (_pid == 0) {
        int fd;

        if (fds[1] == XL_DISPLAYFD)
            (void)fcntl(fds[1], F_SETFD, 0);
        else if (dup2(fds[1], XL_DISPLAYFD) < 0)
            _exit(127);
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        closefrom(XL_DISPLAYFD + 1);
#else
        for (fd = XL_DISPLAYFD + 1; fd < maxfd; fd++)
            (void)close(fd);
#endif
        (void)fd;
        execve(argv[0], argv, envp);
        _exit(127);
    }
    close(fds[1]);
    for (i = 0; argv[i] != NULL; i++)
        free(argv[i]);
    free(argv);
    if (credentials != nil) {
        for (i = 0; envp[i] != NULL; i++)
            if (strncmp(envp[i], "XNEXTBSD_JOIN=", 14) == 0)
                free(envp[i]);
    }
    free(envp);
    XLServerChildPid = _pid > 0 ? _pid : 0;
    if (_pid < 0) {
        *error = [NSString stringWithFormat:@"Can't start the X server: %s",
                  strerror(errno)];
        close(fds[0]);
        [self removeServerAuth];
        return NO;
    }

    // "N\n" on fd 3 once the server listens; EOF if it dies first.
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    while (len < sizeof(buf) - 1) {
        ssize_t r;
        int p = poll(&pfd, 1, XL_START_TIMEOUT_MS);

        if (p < 0 && errno == EINTR)
            continue;
        if (p <= 0)
            break;
        r = read(fds[0], buf + len, sizeof(buf) - 1 - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += (size_t)r;
        if (memchr(buf, '\n', len) != NULL)
            break;
    }
    close(fds[0]);
    buf[len] = '\0';
    if (len == 0 || buf[0] < '0' || buf[0] > '9') {
        if (reaped(_pid))
            *error = @"The X server quit while starting. Its messages "
                     "are in XLibre's log (standard error).";
        else
            *error = @"The X server didn't say which display it took.";
        [self stop];
        return NO;
    }
    _display = [NSString stringWithFormat:@":%d", atoi(buf)];

    // Clients find the cookie in the user's Xauthority, under the display.
    _authority = [[self class] userAuthority];
    _addedCookie = [self xauth:@[@"-q", @"-f", _authority, @"add",
                                 _display, @".", cookie]];
    if (!_addedCookie) {
        // Clients can still use the server's own file.
        _authority = _serverAuth;
    }
    NSLog(@"[XLibre] Xnextbsd (pid %d) is up on display %@", (int)_pid, _display);
    return YES;
}

#pragma mark - Stopping

// Reap the server if it has exited. NSTask's SIGCHLD handling may have
// reaped it already (it waits for any child), which waitpid reports as
// ECHILD.
static BOOL
reaped(pid_t pid)
{
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);

    return r == pid || (r < 0 && errno == ECHILD);
}

- (BOOL)isRunning
{
    if (_pid <= 0)
        return NO;
    if (reaped(_pid)) {
        _pid = 0;
        return NO;
    }
    return YES;
}

- (void)removeServerAuth
{
    serverAuthFile[0] = '\0';
    serverAuthDir[0] = '\0';
    if (_serverAuth != nil)
        unlink([_serverAuth fileSystemRepresentation]);
    if (_serverAuthDir != nil)
        rmdir([_serverAuthDir fileSystemRepresentation]);
    _serverAuth = nil;
    _serverAuthDir = nil;
}

- (void)stop
{
    int status, i;

    XLServerChildPid = 0;
    if (_pid > 0) {
        kill(_pid, SIGTERM);
        for (i = 0; i < 50; i++) {
            if (reaped(_pid)) {
                _pid = 0;
                break;
            }
            usleep(100000);
        }
        if (_pid > 0) {
            kill(_pid, SIGKILL);
            waitpid(_pid, &status, 0);
            _pid = 0;
        }
    }
    if (_addedCookie && _display != nil)
        [self xauth:@[@"-q", @"-f", _authority, @"remove", _display]];
    _addedCookie = NO;
    [self removeServerAuth];
}

@end
