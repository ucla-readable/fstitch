#ifndef KUDOS_KFS_DEBUG_OPCODE_H
#define KUDOS_KFS_DEBUG_OPCODE_H

/* modules */
#define KDB_MODULE_BDESC           100
#define KDB_MODULE_CHDESC_ALTER    200
#define KDB_MODULE_CHDESC_INFO     300

/* opcodes */

/* bdesc (1xx) */
#define KDB_BDESC_ALLOC            101
#define KDB_BDESC_ALLOC_WRAP       102
#define KDB_BDESC_RETAIN           103
#define KDB_BDESC_RELEASE          104
#define KDB_BDESC_DESTROY          105
#define KDB_BDESC_FREE_DDESC       106
#define KDB_BDESC_AUTORELEASE      107
#define KDB_BDESC_AR_POOL_PUSH     108
#define KDB_BDESC_AR_POOL_POP      109

/* chdesc alter (2xx) */
#define KDB_CHDESC_CREATE_NOOP     201
#define KDB_CHDESC_CREATE_BIT      202
#define KDB_CHDESC_CREATE_BYTE     203
#define KDB_CHDESC_APPLY           204
#define KDB_CHDESC_ROLLBACK        205
#define KDB_CHDESC_SET_FLAGS       206
#define KDB_CHDESC_CLEAR_FLAGS     207
#define KDB_CHDESC_DESTROY         208
#define KDB_CHDESC_ADD_DEPENDENCY  209
#define KDB_CHDESC_ADD_DEPENDENT   210
#define KDB_CHDESC_REM_DEPENDENCY  211
#define KDB_CHDESC_REM_DEPENDENT   212
#define KDB_CHDESC_WEAK_RETAIN     213
#define KDB_CHDESC_WEAK_FORGET     214
#define KDB_CHDESC_NOOP_REASSIGN   215

#define KDB_CHDESC_PUSH_DOWN        90
#define KDB_CHDESC_MOVE             91

/* chdesc info (3xx) */
#define KDB_CHDESC_OVERLAP_ATTACH       301
#define KDB_CHDESC_OVERLAP_MULTIATTACH  302
#define KDB_CHDESC_SATISFY              303
#define KDB_CHDESC_WEAK_COLLECT         304
#define KDB_CHDESC_ROLLBACK_COLLECTION  305
#define KDB_CHDESC_APPLY_COLLECTION     306
#define KDB_CHDESC_ORDER_DESTROY        307
#define KDB_CHDESC_DETACH_DEPENDENCIES  308
#define KDB_CHDESC_DETACH_DEPENDENTS    309
#define KDB_CHDESC_DUPLICATE            310
#define KDB_CHDESC_SPLIT                311
#define KDB_CHDESC_MERGE                312

#endif /* KUDOS_KFS_DEBUG_OPCODE_H */
