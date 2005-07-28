import java.io.DataInput;
import java.io.IOException;

class BdescAllocFactory extends ModuleOpcodeFactory
{
	public BdescAllocFactory(DataInput input)
	{
		super(input, KDB_BDESC_ALLOC, "KDB_BDESC_ALLOC");
		addParameter("block", 4);
		addParameter("ddesc", 4);
		addParameter("number", 4);
	}
	
	public BdescAlloc readBdescAlloc() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescAlloc();
	}
}

public class BdescAlloc extends Opcode
{
	public BdescAlloc(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescAllocFactory(input);
	}
}
