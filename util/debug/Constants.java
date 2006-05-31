public interface Constants
{
	/* These constants should be kept in sync with the ones in kfs/debug_opcodes.h */
	public static final short KDB_MODULE_INFO = 1;
	public static final short KDB_MODULE_BDESC = 100;
	public static final short KDB_MODULE_CHDESC_ALTER = 200;
	public static final short KDB_MODULE_CHDESC_INFO = 300;
	
	public static final short KDB_INFO_MARK = 0;
	public static final short KDB_INFO_BD_NAME = 1;
	public static final short KDB_INFO_BDESC_NUMBER = 2;
	public static final short KDB_INFO_CHDESC_LABEL = 3;
	
	public static final short KDB_BDESC_ALLOC = 101;
	public static final short KDB_BDESC_ALLOC_WRAP = 102;
	public static final short KDB_BDESC_RETAIN = 103;
	public static final short KDB_BDESC_RELEASE = 104;
	public static final short KDB_BDESC_DESTROY = 105;
	public static final short KDB_BDESC_FREE_DDESC = 106;
	public static final short KDB_BDESC_AUTORELEASE = 107;
	public static final short KDB_BDESC_AR_RESET = 108;
	public static final short KDB_BDESC_AR_POOL_PUSH = 109;
	public static final short KDB_BDESC_AR_POOL_POP = 110;
	
	public static final short KDB_CHDESC_CREATE_NOOP = 201;
	public static final short KDB_CHDESC_CREATE_BIT = 202;
	public static final short KDB_CHDESC_CREATE_BYTE = 203;
	public static final short KDB_CHDESC_CONVERT_NOOP = 204;
	public static final short KDB_CHDESC_CONVERT_BIT = 205;
	public static final short KDB_CHDESC_CONVERT_BYTE = 206;
	public static final short KDB_CHDESC_REWRITE_BYTE = 207;
	public static final short KDB_CHDESC_APPLY = 208;
	public static final short KDB_CHDESC_ROLLBACK = 209;
	public static final short KDB_CHDESC_SET_FLAGS = 210;
	public static final short KDB_CHDESC_CLEAR_FLAGS = 211;
	public static final short KDB_CHDESC_DESTROY = 212;
	public static final short KDB_CHDESC_ADD_DEPENDENCY = 213;
	public static final short KDB_CHDESC_ADD_DEPENDENT = 214;
	public static final short KDB_CHDESC_REM_DEPENDENCY = 215;
	public static final short KDB_CHDESC_REM_DEPENDENT = 216;
	public static final short KDB_CHDESC_WEAK_RETAIN = 217;
	public static final short KDB_CHDESC_WEAK_FORGET = 218;
	public static final short KDB_CHDESC_SET_OFFSET = 219;
	public static final short KDB_CHDESC_SET_BLOCK = 220;
	public static final short KDB_CHDESC_SET_OWNER = 221;
	public static final short KDB_CHDESC_SET_FREE_PREV = 222;
	public static final short KDB_CHDESC_SET_FREE_NEXT = 223;
	public static final short KDB_CHDESC_SET_FREE_HEAD = 224;
	
	public static final short KDB_CHDESC_MOVE = 301;
	public static final short KDB_CHDESC_SATISFY = 302;
	public static final short KDB_CHDESC_WEAK_COLLECT = 303;
	public static final short KDB_CHDESC_ROLLBACK_COLLECTION = 304;
	public static final short KDB_CHDESC_APPLY_COLLECTION = 305;
	public static final short KDB_CHDESC_ORDER_DESTROY = 306;
	public static final short KDB_CHDESC_DETACH_DEPENDENCIES = 307;
	public static final short KDB_CHDESC_DETACH_DEPENDENTS = 308;
	public static final short KDB_CHDESC_OVERLAP_ATTACH = 309;
	public static final short KDB_CHDESC_OVERLAP_MULTIATTACH = 310;
	public static final short KDB_CHDESC_DUPLICATE = 311;
	public static final short KDB_CHDESC_SPLIT = 312;
	public static final short KDB_CHDESC_MERGE = 313;
}
