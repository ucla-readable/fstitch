import java.io.DataInput;
//import java.io.IOException;

public class BdescAlloc extends Opcode
{
	private final int block, ddesc, number;
	private final short count;
	
	public BdescAlloc(int block, int ddesc, int number, int count)
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
		return "KDB_BDESC_ALLOC: block = " + SystemState.hex(block) + ", ddesc = " + SystemState.hex(ddesc) + ", number = " + number + ", count = " + count;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_ALLOC, "KDB_BDESC_ALLOC", BdescAlloc.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("number", 4);
		factory.addParameter("count", 4); /* technically a 16-bit */
		return factory;
	}
}
