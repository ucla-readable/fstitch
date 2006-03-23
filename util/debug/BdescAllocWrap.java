public class BdescAllocWrap extends Opcode
{
	private final int block, ddesc, number;
	private final short count;
	
	public BdescAllocWrap(int block, int ddesc, int number, int count)
	{
		this.block = block;
		this.ddesc = ddesc;
		this.number = number;
		this.count = (short) count;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public String toString()
	{
		return "KDB_BDESC_ALLOC_WRAP: block = " + SystemState.hex(block) + ", ddesc = " + SystemState.hex(ddesc) + ", number = " + number + ", count = " + count;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_ALLOC_WRAP, "KDB_BDESC_ALLOC_WRAP", BdescAllocWrap.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("number", 4);
		factory.addParameter("count", 4); /* technically a 16-bit */
		return factory;
	}
}
