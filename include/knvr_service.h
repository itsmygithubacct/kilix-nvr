#ifndef KNVR_SERVICE_H
#define KNVR_SERVICE_H

/*
 * The recorder as something that keeps running.
 *
 * `run` in a terminal stops when the terminal does.  This writes the user
 * units that make it a service and schedules retention alongside it,
 * because a recorder installed without a prune timer is a disk that
 * fills - which is why `record=continuous` was never turned on here.
 *
 * `action` is "install", "remove" or "status"; NULL means install.
 */
int knvr_service(const char *action);

#endif /* KNVR_SERVICE_H */
