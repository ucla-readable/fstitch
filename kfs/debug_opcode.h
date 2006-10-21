#ifndef KUDOS_KFS_DEBUG_OPCODE_H
#define KUDOS_KFS_DEBUG_OPCODE_H

#define DEBUG_OPCODE_REV           "$Rev$"

/* modules */
#define KDB_MODULE_INFO              1
#define KDB_MODULE_BDESC           100
#define KDB_MODULE_CHDESC_ALTER    200
#define KDB_MODULE_CHDESC_INFO     300

/* opcodes */

/* info (0xx) */
#define KDB_INFO_MARK                0
#define KDB_INFO_BD_NAME             1
#define KDB_INFO_BDESC_NUMBER        2
#define KDB_INFO_CHDESC_LABEL        3

/* bdesc (1xx) */
#define KDB_BDESC_ALLOC            101
#define KDB_BDESC_ALLOC_WRAP       102
#define KDB_BDESC_RETAIN           103
#define KDB_BDESC_RELEASE          104
#define KDB_BDESC_DESTROY          105
#define KDB_BDESC_FREE_DDESC       106
#define KDB_BDESC_AUTORELEASE      107
#define KDB_BDESC_AR_RESET         108
#define KDB_BDESC_AR_POOL_PUSH     109
#define KDB_BDESC_AR_POOL_POP      110

/* chdesc alter (2xx) */
#define KDB_CHDESC_CREATE_NOOP     201
#define KDB_CHDESC_CREATE_BIT      202
#define KDB_CHDESC_CREATE_BYTE     203
#define KDB_CHDESC_CONVERT_NOOP    204
#define KDB_CHDESC_CONVERT_BIT     205
#define KDB_CHDESC_CONVERT_BYTE    206
#define KDB_CHDESC_REWRITE_BYTE    207
#define KDB_CHDESC_APPLY           208
#define KDB_CHDESC_ROLLBACK        209
#define KDB_CHDESC_SET_FLAGS       210
#define KDB_CHDESC_CLEAR_FLAGS     211
#define KDB_CHDESC_DESTROY         212
#define KDB_CHDESC_ADD_BEFORE      213
#define KDB_CHDESC_ADD_AFTER       214
#define KDB_CHDESC_REM_BEFORE      215
#define KDB_CHDESC_REM_AFTER       216
#define KDB_CHDESC_WEAK_RETAIN     217
#define KDB_CHDESC_WEAK_FORGET     218
#define KDB_CHDESC_SET_OFFSET      219
#define KDB_CHDESC_SET_LENGTH      220
#define KDB_CHDESC_SET_BLOCK       221
#define KDB_CHDESC_SET_OWNER       222
#define KDB_CHDESC_SET_FREE_PREV   223
#define KDB_CHDESC_SET_FREE_NEXT   224
#define KDB_CHDESC_SET_FREE_HEAD   225

/* chdesc info (3xx) */
#define KDB_CHDESC_SATISFY              301
#define KDB_CHDESC_WEAK_COLLECT         302
#define KDB_CHDESC_DETACH_BEFORES       303
#define KDB_CHDESC_OVERLAP_ATTACH       304
#define KDB_CHDESC_OVERLAP_MULTIATTACH  305

#endif /* KUDOS_KFS_DEBUG_OPCODE_H */
