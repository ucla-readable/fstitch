import java.io.DataInput;
import java.io.IOException;

class ChdescConvertBitFactory extends ModuleOpcodeFactory
{
	public ChdescConvertBitFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CONVERT_BIT);
		addParameter("chdesc", 4);
		addParameter("offset", 2);
		addParameter("xor", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_CONVERT_BIT"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescConvertBit readChdescConvertBit() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescConvertBit();
	}
}

public class ChdescConvertBit extends Opcode
{
	public ChdescConvertBit(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescConvertBitFactory(input);
	}
}
