#ifndef UPLC_VERSION_H
#define UPLC_VERSION_H

#define UPLC_VERSION_MAJOR 0
#define UPLC_VERSION_MINOR 0
#define UPLC_VERSION_PATCH 1
#define UPLC_VERSION_STRING "0.0.1"

/* Magic placed in the .note.uplc section of every .uplcx artifact.
 * 8 bytes total (NUL-padded) so readers can do a single 64-bit compare. */
#define UPLC_ARTIFACT_MAGIC     "UPLCX\0\0\0"
#define UPLC_ARTIFACT_MAGIC_LEN 8

/* Plutus language version range this toolchain emits / accepts. */
#define UPLC_PLUTUS_VERSION_MIN "1.0.0"
#define UPLC_PLUTUS_VERSION_MAX "1.1.0"
#define UPLC_PLUTUS_VERSION_DEFAULT "1.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the runtime version string (UPLC_VERSION_STRING). */
const char* uplcrt_version(void);

#ifdef __cplusplus
}
#endif

#endif /* UPLC_VERSION_H */
