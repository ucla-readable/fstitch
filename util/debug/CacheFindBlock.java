public class CacheFindBlock extends Opcode
{
	private final int bd;
	
	public CacheFindBlock(int bd)
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
		return "KDB_CACHE_FINDBLOCK: bd = " + SystemState.hex(bd);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CACHE_FINDBLOCK, "KDB_CACHE_FINDBLOCK", CacheFindBlock.class);
		factory.addParameter("cache", 4);
		return factory;
	}
}
