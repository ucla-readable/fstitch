import java.io.DataInput;
import java.io.IOException;

class ChdescCreateBitFactory extends ModuleOpcodeFactory
{
	public ChdescCreateBitFactory(DataInput input)
	{
		super(input, KDB_CHDESC_CREATE_BIT, "KDB_CHDESC_CREATE_BIT");
		addParameter("chdesc", 4);
		addParameter("block", 4);
		addParameter("owner", 4);
		addParameter("offset", 2);
		addParameter("xor", 4);
	}
	
	public ChdescCreateBit readChdescCreateBit() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescCreateBit();
	}
}

public class ChdescCreateBit extends Opcode
{
	public ChdescCreateBit(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescCreateBitFactory(input);
	}
}
