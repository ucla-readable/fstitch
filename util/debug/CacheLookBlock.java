public class CacheLookBlock extends Opcode
{
	private final int bd, block;
	
	public CacheLookBlock(int bd, int block)
	{
		this.bd = bd;
		this.block = block;
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
		return "KDB_CACHE_LOOKBLOCK: bd = " + SystemState.hex(bd) + ", block = " + block;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CACHE_LOOKBLOCK, "KDB_CACHE_LOOKBLOCK", CacheLookBlock.class);
		factory.addParameter("bd", 4);
		factory.addParameter("block", 4);
		return factory;
	}
}
