import java.io.DataInput;
import java.io.IOException;

class ChdescCreateByteFactory extends ModuleOpcodeFactory
{
	public ChdescCreateByteFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CREATE_BYTE, "KDB_CHDESC_CREATE_BYTE");
		addParameter("chdesc", 4);
		addParameter("block", 4);
		addParameter("owner", 4);
		addParameter("offset", 2);
		addParameter("length", 2);
	}
	
	public ChdescCreateByte readChdescCreateByte() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescCreateByte();
	}
}

public class ChdescCreateByte extends Opcode
{
	public ChdescCreateByte(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescCreateByteFactory(input);
	}
}
