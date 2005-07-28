import java.io.DataInput;
import java.io.IOException;

class ChdescSetFlagsFactory extends ModuleOpcodeFactory
{
	public ChdescSetFlagsFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SET_FLAGS, "KDB_CHDESC_SET_FLAGS");
		addParameter("chdesc", 4);
		addParameter("flags", 4);
	}
	
	public ChdescSetFlags readChdescSetFlags() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescSetFlags();
	}
}

public class ChdescSetFlags extends Opcode
{
	public ChdescSetFlags(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescSetFlagsFactory(input);
	}
}
