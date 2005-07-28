import java.io.DataInput;
import java.io.IOException;

class BdescRetainFactory extends ModuleOpcodeFactory
{
	public BdescRetainFactory(DataInput input)
	{
		super(input, KDB_BDESC_RETAIN);
		addParameter("block", 4);
		addParameter("ddesc", 4);
		addParameter("ref_count", 4);
		addParameter("ar_count", 4);
		addParameter("dd_count", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_BDESC_RETAIN"))
			throw new UnexpectedNameException(name);
	}
	
	public BdescRetain readBdescRetain() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescRetain();
	}
}

public class BdescRetain extends Opcode
{
	public BdescRetain(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescRetainFactory(input);
	}
}
