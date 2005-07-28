import java.io.DataInput;
import java.io.IOException;

class ChdescConvertByteFactory extends ModuleOpcodeFactory
{
	public ChdescConvertByteFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CONVERT_BYTE, "KDB_CHDESC_CONVERT_BYTE");
		addParameter("chdesc", 4);
		addParameter("offset", 2);
		addParameter("length", 2);
	}
	
	public ChdescConvertByte readChdescConvertByte() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescConvertByte();
	}
}

public class ChdescConvertByte extends Opcode
{
	public ChdescConvertByte(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescConvertByteFactory(input);
	}
}
