/*
 * Surviving a logout.
 *
 * `run` is a foreground process, which is the right shape for something a
 * person starts and watches and the wrong shape for a recorder: close the
 * terminal and the cameras stop.  This writes the two user units that fix
 * that - the recorder, and an hourly prune - and hands them to systemd.
 *
 * User units, not system ones: nothing here runs as root, the data lives
 * under the user's home, and a recorder that needed a privileged install
 * would be a recorder nobody could try.  The cost is that user units stop
 * at logout unless lingering is enabled, which needs one privileged
 * command - so this checks and says so rather than pretending.
 *
 * Retention is installed at the same time and deliberately not
 * separately: `prune` is a command with no schedule, and continuous
 * recording with nothing deleting anything is a full disk.  A recorder
 * you can start without arranging for that is a trap.
 */

#include "knvr_service.h"

#include "knvr_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define UNIT_RECORD "kilix-nvr.service"
#define UNIT_PRUNE "kilix-nvr-prune.service"
#define UNIT_TIMER "kilix-nvr-prune.timer"

/* Run a command, inheriting stdio, and report whether it succeeded.  No
 * shell: every argument here comes from this file, and keeping it that
 * way means it stays true when a path acquires a space. */
static bool run_quietly(const char *const argv[], bool show)
{
    pid_t child = fork();
    int status = 0;

    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if (!show) {
            const int null_fd = open("/dev/null", O_WRONLY);

            if (null_fd >= 0) {
                (void)dup2(null_fd, STDOUT_FILENO);
                (void)dup2(null_fd, STDERR_FILENO);
                (void)close(null_fd);
            }
        }
        (void)execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        continue;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool systemctl(const char *verb, const char *unit, bool show)
{
    const char *argv[6];
    size_t count = 0u;

    argv[count++] = "systemctl";
    argv[count++] = "--user";
    argv[count++] = verb;
    if (unit != NULL) {
        argv[count++] = unit;
    }
    argv[count] = NULL;
    return run_quietly(argv, show);
}

static bool unit_dir(char *out, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (config != NULL && config[0] == '/') {
        if (snprintf(out, size, "%s/systemd/user", config) < 0) {
            return false;
        }
    } else if (home != NULL && home[0] == '/') {
        if (snprintf(out, size, "%s/.config/systemd/user", home) < 0) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

/* This executable, by absolute path.  A unit that said `kilix-nvr` would
 * depend on a PATH systemd does not have. */
static bool self_path(char *out, size_t size)
{
    const ssize_t length = readlink("/proc/self/exe", out, size - 1u);

    if (length <= 0) {
        return false;
    }
    out[length] = '\0';
    return true;
}

static bool write_unit(const char *directory, const char *name,
                       const char *body)
{
    char path[KNVR_PATH_MAX];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) < 0) {
        return false;
    }
    file = fopen(path, "w");
    if (file == NULL) {
        (void)fprintf(stderr, "kilix-nvr: cannot write %s\n", path);
        return false;
    }
    (void)fputs(body, file);
    if (fclose(file) != 0) {
        return false;
    }
    (void)printf("wrote %s\n", path);
    return true;
}

/*
 * Whether this user's services keep running after they log out.
 *
 * Reported rather than fixed: enabling it needs root, and a program that
 * asked for a password to finish installing itself would be a program
 * that gets installed with sudo.  Saying the exact command is enough.
 */
static void report_lingering(void)
{
    const char *user = getenv("USER");
    char path[KNVR_PATH_MAX];
    struct stat info;

    if (user == NULL || user[0] == '\0') {
        return;
    }
    if (snprintf(path, sizeof(path), "/var/lib/systemd/linger/%s", user) < 0) {
        return;
    }
    if (stat(path, &info) == 0) {
        return;
    }
    (void)printf("\nnote: user services stop when you log out.  To keep the\n"
                 "      recorder running across logouts:\n"
                 "        sudo loginctl enable-linger %s\n",
                 user);
}

static int install(void)
{
    char directory[KNVR_PATH_MAX];
    char config[KNVR_PATH_MAX];
    char exe[KNVR_PATH_MAX];
    char body[4096];
    const char *env_file = NULL;

    if (!unit_dir(directory, sizeof(directory))) {
        (void)fprintf(stderr, "kilix-nvr: cannot locate the unit directory\n");
        return 1;
    }
    if (!self_path(exe, sizeof(exe))) {
        (void)fprintf(stderr, "kilix-nvr: cannot locate this executable\n");
        return 1;
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "kilix-nvr: cannot create %s\n", directory);
        return 1;
    }
    /*
     * Kilix's settings file, if there is one, is where the detector and
     * the classifier are recorded.  Without it a service records and
     * never infers - which is exactly the failure this whole afternoon
     * was: a setting that exists and is not reaching the process.  The
     * leading `-` makes it optional, since a machine with no models
     * should still record.
     */
    {
        const char *home = getenv("GPU_TERMINAL_DATA_HOME");
        char candidate[KNVR_PATH_MAX];
        struct stat info;

        if (home == NULL || home[0] == '\0') {
            const char *user_home = getenv("HOME");

            if (user_home != NULL &&
                snprintf(candidate, sizeof(candidate),
                         "%s/.local/gpu_terminal/kilix/config/kilix.env",
                         user_home) > 0 &&
                stat(candidate, &info) == 0) {
                env_file = candidate;
            }
        } else if (snprintf(candidate, sizeof(candidate),
                            "%s/kilix/config/kilix.env", home) > 0 &&
                   stat(candidate, &info) == 0) {
            env_file = candidate;
        }
        if (env_file != NULL) {
            (void)snprintf(config, sizeof(config), "%s", env_file);
            env_file = config;
        }
    }

    if (snprintf(body, sizeof(body),
                 "[Unit]\n"
                 "Description=kilix-nvr: record every configured camera\n"
                 "Documentation=man:kilix-nvr\n"
                 "After=network-online.target\n"
                 "Wants=network-online.target\n"
                 "\n"
                 "[Service]\n"
                 "Type=simple\n"
                 "ExecStart=%s run\n"
                 "%s%s%s"
                 /* Cameras drop out, ffmpeg dies, a model wedges.  A
                  * recorder that gives up on the first of those is not a
                  * recorder; the delay keeps a genuinely broken config
                  * from spinning. */
                 "Restart=always\n"
                 "RestartSec=10\n"
                 /* SIGINT rather than SIGTERM would work too - it handles
                  * both - but TERM is what systemd sends by default and
                  * the runner closes its open events on either. */
                 "KillSignal=SIGTERM\n"
                 "TimeoutStopSec=30\n"
                 "NoNewPrivileges=true\n"
                 "\n"
                 "[Install]\n"
                 "WantedBy=default.target\n",
                 exe,
                 env_file != NULL ? "EnvironmentFile=-" : "",
                 env_file != NULL ? env_file : "",
                 env_file != NULL ? "\n" : "") < 0) {
        return 1;
    }
    if (!write_unit(directory, UNIT_RECORD, body)) {
        return 1;
    }

    if (snprintf(body, sizeof(body),
                 "[Unit]\n"
                 "Description=kilix-nvr: apply retention\n"
                 "\n"
                 "[Service]\n"
                 "Type=oneshot\n"
                 "ExecStart=%s prune\n"
                 "NoNewPrivileges=true\n",
                 exe) < 0) {
        return 1;
    }
    if (!write_unit(directory, UNIT_PRUNE, body)) {
        return 1;
    }

    if (snprintf(body, sizeof(body),
                 "[Unit]\n"
                 "Description=kilix-nvr: apply retention hourly\n"
                 "\n"
                 "[Timer]\n"
                 /* Hourly, not daily: retention is a per-camera age and a
                  * global size cap, and a cap checked once a day is a cap
                  * that can be a day late on a disk that fills in an
                  * afternoon. */
                 "OnCalendar=hourly\n"
                 "RandomizedDelaySec=300\n"
                 /* So a machine that was off does not simply skip a day's
                  * retention. */
                 "Persistent=true\n"
                 "\n"
                 "[Install]\n"
                 "WantedBy=timers.target\n") < 0) {
        return 1;
    }
    if (!write_unit(directory, UNIT_TIMER, body)) {
        return 1;
    }

    if (!systemctl("daemon-reload", NULL, true)) {
        (void)fprintf(stderr,
                      "kilix-nvr: systemctl --user is not available; the "
                      "units are written but not started\n");
        return 1;
    }
    /* Two steps rather than `enable --now`, so the failure of each is
     * its own. */
    (void)systemctl("enable", UNIT_TIMER, false);
    (void)systemctl("start", UNIT_TIMER, false);
    if (!systemctl("enable", UNIT_RECORD, true) ||
        !systemctl("restart", UNIT_RECORD, true)) {
        (void)fprintf(stderr, "kilix-nvr: could not start %s\n", UNIT_RECORD);
        return 1;
    }
    (void)printf("\n%s and %s are enabled\n", UNIT_RECORD, UNIT_TIMER);
    report_lingering();
    return 0;
}

static int remove_units(void)
{
    char directory[KNVR_PATH_MAX];
    const char *const units[] = {UNIT_RECORD, UNIT_PRUNE, UNIT_TIMER};

    if (!unit_dir(directory, sizeof(directory))) {
        return 1;
    }
    (void)systemctl("stop", UNIT_RECORD, false);
    (void)systemctl("disable", UNIT_RECORD, false);
    (void)systemctl("stop", UNIT_TIMER, false);
    (void)systemctl("disable", UNIT_TIMER, false);
    for (size_t i = 0u; i < sizeof(units) / sizeof(units[0]); i++) {
        char path[KNVR_PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", directory, units[i]) < 0) {
            continue;
        }
        if (remove(path) == 0) {
            (void)printf("removed %s\n", path);
        }
    }
    (void)systemctl("daemon-reload", NULL, false);
    /* The data is left alone on purpose: removing the service must not
     * remove the footage. */
    (void)printf("\nthe recordings and the event store are untouched\n");
    return 0;
}

static int status(void)
{
    (void)systemctl("status", UNIT_RECORD, true);
    (void)systemctl("list-timers", UNIT_TIMER, true);
    report_lingering();
    return 0;
}

int knvr_service(const char *action)
{
    if (action == NULL || strcmp(action, "install") == 0) {
        return install();
    }
    if (strcmp(action, "remove") == 0) {
        return remove_units();
    }
    if (strcmp(action, "status") == 0) {
        return status();
    }
    (void)fprintf(stderr,
                  "kilix-nvr: service takes install, remove or status\n");
    return 2;
}
