import java.io.DataInput;
import java.io.IOException;

class BdescDestroyFactory extends ModuleOpcodeFactory
{
	public BdescDestroyFactory(DataInput input)
	{
		super(input, KDB_BDESC_DESTROY);
		addParameter("block", 4);
		addParameter("ddesc", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_BDESC_DESTROY"))
			throw new UnexpectedNameException(name);
	}
	
	public BdescDestroy readBdescDestroy() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescDestroy();
	}
}

public class BdescDestroy extends Opcode
{
	public BdescDestroy(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescDestroyFactory(input);
	}
}
