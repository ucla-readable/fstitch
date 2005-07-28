import java.io.DataInput;
import java.io.IOException;

class BdescReleaseFactory extends ModuleOpcodeFactory
{
	public BdescReleaseFactory(DataInput input)
	{
		super(input, KDB_BDESC_RELEASE);
		addParameter("block", 4);
		addParameter("ddesc", 4);
		addParameter("ref_count", 4);
		addParameter("ar_count", 4);
		addParameter("dd_count", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_BDESC_RELEASE"))
			throw new UnexpectedNameException(name);
	}
	
	public BdescRelease readBdescRelease() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readBdescRelease();
	}
}

public class BdescRelease extends Opcode
{
	public BdescRelease(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new BdescReleaseFactory(input);
	}
}
