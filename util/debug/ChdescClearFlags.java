import java.io.DataInput;
import java.io.IOException;

class ChdescClearFlagsFactory extends ModuleOpcodeFactory
{
	public ChdescClearFlagsFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CLEAR_FLAGS);
		addParameter("chdesc", 4);
		addParameter("flags", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_CLEAR_FLAGS"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescClearFlags readChdescClearFlags() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescClearFlags();
	}
}

public class ChdescClearFlags extends Opcode
{
	public ChdescClearFlags(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescClearFlagsFactory(input);
	}
}
