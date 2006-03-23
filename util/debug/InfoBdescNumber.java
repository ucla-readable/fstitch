public class InfoBdescNumber extends Opcode
{
	private final int block, number;
	private final short count;
	
	public InfoBdescNumber(int block, int number, int count)
	{
		this.block = block;
		this.number = number;
		this.count = (short) count;
	}
	
	public void applyTo(SystemState state)
	{
		state.setBdesc(block, number, count);
	}
	
	public boolean isSkippable()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_INFO_BDESC_NUMBER: block = " + SystemState.hex(block) + ", number = " + number + ", count = " + count;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_BDESC_NUMBER, "KDB_INFO_BDESC_NUMBER", InfoBdescNumber.class);
		factory.addParameter("block", 4);
		factory.addParameter("number", 4);
		factory.addParameter("count", 4); /* technically a 16-bit */
		return factory;
	}
}
