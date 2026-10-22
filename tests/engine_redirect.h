#ifndef LIZARD_TEST_ENGINE_REDIRECT_H
#define LIZARD_TEST_ENGINE_REDIRECT_H

/*
 * Force-included (-include) into the *_ge variants of the VM tests. Every call
 * a test makes to the reference interpreter is routed through the Gecko Engine
 * instead, so the whole existing suite doubles as a conformance test for GE:
 * the same assertions, the same expected output, errors and fuel behaviour.
 */
#include "lizard/engine.h"

#define lz_vm_run lz_ge_run
#define lz_vm_eval lz_ge_eval

#endif
