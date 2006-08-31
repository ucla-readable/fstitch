public class ChdescOverlapMultiattach extends Opcode
{
	private final int chdesc, block;
	
	public ChdescOverlapMultiattach(int chdesc, int block)
	{
		this.chdesc = chdesc;
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
		return "KDB_CHDESC_OVERLAP_MULTIATTACH: chdesc = " + SystemState.hex(chdesc) + ", block = " + SystemState.hex(block);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_OVERLAP_MULTIATTACH, "KDB_CHDESC_OVERLAP_MULTIATTACH", ChdescOverlapMultiattach.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		return factory;
	}
}
