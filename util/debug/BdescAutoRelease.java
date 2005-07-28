import java.io.DataInput;
import java.io.IOException;

class BdescAutoReleaseFactory extends ModuleOpcodeFactory
{
	public BdescAutoReleaseFactory(DataInput input)
	{
		super(input, KDB_BDESC_AUTORELEASE);
		addParameter("block", 4);
		addParameter("ddesc", 4);
		addParameter("ref_count", 4);
		addParameter("ar_count", 4);
		addParameter("dd_count", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_BDESC_AUTORELEASE"))
			throw new UnexpectedNameException(name);
	}
	
	public BdescAutoRelease readBdescAutoRelease() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescAutoRelease();
	}
}

public class BdescAutoRelease extends Opcode
{
	public BdescAutoRelease(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescAutoReleaseFactory(input);
	}
}
