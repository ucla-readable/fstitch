import java.io.DataInput;
import java.io.IOException;

class BdescAllocWrapFactory extends ModuleOpcodeFactory
{
	public BdescAllocWrapFactory(DataInput input)
	{
		super(input, KDB_BDESC_ALLOC_WRAP, "KDB_BDESC_ALLOC_WRAP");
		addParameter("block", 4);
		addParameter("ddesc", 4);
		addParameter("number", 4);
	}
	
	public BdescAllocWrap readBdescAllocWrap() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescAllocWrap();
	}
}

public class BdescAllocWrap extends Opcode
{
	public BdescAllocWrap(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescAllocWrapFactory(input);
	}
}
