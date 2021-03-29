#include <ftw.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define werr(...) { fprintf(stderr, __VA_ARGS__); fflush(stderr); }

#define PROG           "dpl"
#define GIT_PATH       "/usr/bin/git"
#define LBU_CONF       "/etc/lbu/lbu.conf"
#define PERMS_FILE     "perms.bkp"
#define POST_DPL       "post.dpl"
#define MAX_DEPTH      15

static void usage(int rc, char *msg, ...) {
  if (rc >= 100) {
    perror(msg);
    exit(rc);
  }

  if (msg) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    werr("\n");
  }

  if (rc > 1)
    exit(rc);

  werr(
    "USAGE:\n"
    "  As a shell command:\n"
    "    $ dpl\n"
    "\n"
    "  As a symbolic link:\n"
    "    $ ln -s dpl 'dpl*v1.12*srv%%www%%site.com'\n"
    "\n"
    "  Extra git refs PREV and CRNT:\n"
    "    $ cat revert.sh\n"
    "    #!/bin/sh\n"
    "    echo Reverting...\n"
    "    exec -a dpl*PREV* dpl\n"
    "\n"
    "FILES:\n"
    "  /etc/lbu/lbu.conf    Configuration variables\n"
    "  /perms.bkp           Permissions shell script generated by \"bkp\" tool\n"
    "  post.dpl             Post deploy hook\n"
    "\n"
    "ENVIRONMENT:\n"
    "  DPL_STORE      Git repository to checkout from\n"
    "  DPL_DEST       Directory to checkout into a.k.a working tree (i.e. \"/\")\n"
    "  DPL_REV        Revision to checkout (\"HEAD\" by default; can be any git revspec)\n"
    "  DPL_PATHS      Paths to checkout (i.e \"/etc:/home/bender:/srv/www:/perms.bkp\")\n");

  exit(rc);
}

void readconf() {
  char buf[100];
  FILE *file = fopen(LBU_CONF, "r");
  if (file == NULL)
    return;

  while(fgets(buf, sizeof(buf), file)) {
    if (memcmp(buf, "#", 1) == 0 ||
        memcmp(buf, "\n", 1) == 0 ||
        memcmp(buf, " ", 1) == 0)
      continue;

    // trim newline byte
    char *c = strchr(buf, '\n');
    if (c)
      *c = 0;

    char *kv = malloc(strlen(buf) + 1);
    strcpy(kv, buf);

    if (getenv(strtok(buf, "=")) == NULL)
      putenv(kv);
  }

  fclose(file);
}

int exec(const char *msg, char **args) {
  werr("%s\n", msg);

  pid_t pid = fork();
  if (pid == -1)
  {
    perror(args[0]);
    exit(102);
  }
  else if (pid > 0)
  {
    int status;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
  }
  else
  {
    execve(args[0], args, NULL);

    // exec must not return
    exit(1);
  }
}

int git(char *arg1, ...) {
  char *argv[20] = { GIT_PATH, arg1 };
  int idx = 1;
  char *arg;
  char *s;

  va_list ap;
  va_start(ap, arg1);

  // last argument must start from `>> `
  // some arguments may look as a space separated line
  while (strncmp((arg = va_arg(ap, char *)), ">> ", 3) != 0)
    for (arg = strtok_r(arg, " ", &s);
         arg != NULL;
         arg = strtok_r(NULL, " ", &s))
      argv[++idx] = arg;

  va_end(ap);

  // `arg` now points to the last argument
  return exec(arg, argv);
}

char* pathspec(char *str) {
  if (str == NULL || memcmp(str, "/", 1) != 0)
    return NULL;

  int bufsiz = 0;
  int n = 2; // first `:` symbol and last `\0`
  char *c = str;
  // nice as hell bufsize calculation
  // /etc/lbu:/root/.profile -> :/etc/lbu :/root/.profile
  while (*c++ != '\0')
    n += *c == ':' ? 2 : 1;

  char *path = malloc(n);
  memset(path, '\0', n);

  char *p = path;
  *p++ = ':';
  c = str;
  while (*c != '\0') {
    if (*c == ':') {
      *p++ = ' ';
      *p = ':';
    }
    else if (*c == '%') {
      *p = '/';
    }
    else {
      *p = *c;
    }

    p++;
    c++;
  }

  return path;
}

int samepath(char *real, char *arg) {
  if (real == NULL || arg == NULL)
    return 0;

  if (memcmp(arg, "/", 1) == 0)
    return strcmp(real, arg) == 0;

  struct passwd *pw = getpwuid(geteuid());
  if (pw == NULL)
    return 0;

  char *home = pw->pw_dir;
  char *full = malloc(strlen(home) + 1 + strlen(arg) + 1);
  strcpy(full, home);
  strcat(full, "/");
  strcat(full, arg);

  return strcmp(real, full) == 0;
}

void git_receive_pack(char *bkp, int argc, char **argv) {
  if (getenv("SSH_CONNECTION") == NULL)
    return;

  if (argc > 1 && !samepath(bkp, argv[1]))
      usage(1, "Wrong argument(s) or not a git-push call.");

  if (git("receive-pack", bkp, ">> git-receive-pack"))
    usage(101, "git-receive-pack");

  // git-receive-pack closes stdout
  // reopening it for the next steps
  dup2(2, 1);
}

char *getrev() {
  char *rev = getenv("DPL_REV");

  if (rev == NULL)
    return "HEAD";

  return rev;
}

char *getpth() {
  char *dir = pathspec(getenv("DPL_PATHS"));

  if (dir == NULL)
    usage(1, "DPL_PATHS not found in %s or environment variables.", LBU_CONF);

  return dir;
}

char* getbkp() {
  char *dir = getenv("DPL_STORE");

  if (dir == NULL)
    usage(1, "DPL_STORE not found in %s or environment variables.", LBU_CONF);

  return dir;
}

char* getdst() {
  char *dir = getenv("DPL_DEST");

  if (dir == NULL)
    usage(1, "DPL_DEST must be explicitly set in %s or environment variable.", LBU_CONF);

  return dir;
}

void readarg0(char *arg0, char **rev, char **pth) {
  // take the last path segment
  char *fname = strrchr(arg0, '/');
  if (fname++ == NULL)
    fname = arg0;

  char *s;
  // expected:
  // - dpl
  // - dpl*HEAD*%etc%resolve.conf:%etc%lbu
  // - dpl*PREV
  // - dpl*test-case-ID123*%srv%site.com
  if (memcmp(strtok_r(fname, "*", &s), PROG, strlen(PROG)))
    usage(1, "Name of executable/link must begin from %s.", PROG);

  char *tok;
  if (tok = strtok_r(NULL, "*", &s))
    *rev = tok;

  if (tok = pathspec(strtok_r(NULL, "\0", &s)))
    *pth = tok;
}

int do_entry(
    const char *path,
    const struct stat *fstat,
    const int type,
    struct FTW *ftw)
{
  char *match;
  if (type == FTW_F &&
      (match = strstr(path, POST_DPL)) &&
      strcmp(POST_DPL, match) == 0 &&
      access(path, X_OK) == 0)
        exec(path, (const char *[]) { path, 0 });

  return 0;
}

void post_dpl() {
  werr(">> looking for post.dpl...\n");

  int res = nftw(".", do_entry, MAX_DEPTH, FTW_PHYS);

  if (res != 0)
    werr("warning: %s\n", strerror(res));
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "-h") == 0)
    usage(0, NULL);

  // enrich environ
  readconf();

  // set defaults
  char *rev = getrev(),
       *pth = getpth(),
       *bkp = getbkp(),
       *dst = getdst();

  // symlink?
  readarg0(argv[0], &rev, &pth);

  // unprivileged
  if (setreuid(geteuid(), getuid()) < 0)
    usage(101, "setreuid");

  // handle git-pull request if needed
  git_receive_pack(bkp, argc, argv);

  werr(
    ">> rev=%s\n"
    "   pth=%s\n"
    "   bkp=%s\n"
    "   dst=%s\n", rev, pth, bkp, dst);

  if (chdir(bkp))
    usage(101, "chdir");

  if (git("rev-parse", "--verify", "--quiet", rev, ">> git-rev-parse <rev>"))
    usage(101, "git-rev-parse");

  // privileged
  if (setreuid(geteuid(), getuid()) < 0)
    usage(101, "setreuid");

  if (git("--work-tree", dst, "restore", "--worktree", "--source", rev, "--", pth,
        ">> git-restore <pth> of <rev> from <bkp> into <dst>"))
    usage(101, "git-restore");

  // unprivileged
  if (setreuid(geteuid(), getuid()) < 0)
    usage(101, "setreuid");

  git("update-ref", "PREV", "CRNT", ">> git-update-ref PREV");
  git("update-ref", "CRNT", "HEAD", ">> git-update-ref CRNT");

  // (!) git-restore does not update HEAD as git-checkout does
  //
  // restoring HEAD since checkout affects it
  // git("symbolic-ref", "HEAD", "refs/heads/master", ">> git-symbolic-ref HEAD master");

  if (chdir(dst))
    exit(0);

  // This is a MUST for shell script execution
  setuid(0);

  if (access(PERMS_FILE, X_OK) == 0 &&
      exec(">> restoring permissions", (char *[]) { PERMS_FILE, 0 }) == 0)
    unlink(PERMS_FILE);

  post_dpl();

  exit(0);
}
