public class CacheNotify extends Opcode
{
	private final int bd;
	
	public CacheNotify(int bd)
	{
		this.bd = bd;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CACHE_NOTIFY: bd = " + SystemState.hex(bd);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CACHE_NOTIFY, "KDB_CACHE_NOTIFY", CacheNotify.class);
		factory.addParameter("bd", 4);
		return factory;
	}
}
