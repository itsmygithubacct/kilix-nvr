#ifndef KNVR_REVIEW_H
#define KNVR_REVIEW_H

/*
 * The review interface: what happened, with the picture that caused it.
 *
 * Separate from the store so the store stays free of a terminal, and so
 * the list of events can be queried by anything that is not this.
 */

#include "knvr_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs until the operator quits.  Returns a process exit status. */
int knvr_review(knvr_store *store);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_REVIEW_H */
