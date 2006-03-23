public class ChdescOverlapMultiattach extends Opcode
{
	private final int chdesc, block;
	private final boolean slip_under;
	
	public ChdescOverlapMultiattach(int chdesc, int block, byte slip_under)
	{
		this.chdesc = chdesc;
		this.block = block;
		this.slip_under = slip_under != 0;
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
		return "KDB_CHDESC_OVERLAP_MULTIATTACH: chdesc = " + SystemState.hex(chdesc) + ", block = " + SystemState.hex(block) + ", slip_under = " + slip_under;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_OVERLAP_MULTIATTACH, "KDB_CHDESC_OVERLAP_MULTIATTACH", ChdescOverlapMultiattach.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("slip_under", 1);
		return factory;
	}
}
