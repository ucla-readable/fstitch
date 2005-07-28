import java.io.DataInput;
import java.io.IOException;

class ChdescSetBlockFactory extends ModuleOpcodeFactory
{
	public ChdescSetBlockFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SET_BLOCK);
		addParameter("chdesc", 4);
		addParameter("block", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_SET_BLOCK"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescSetBlock readChdescSetBlock() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescSetBlock();
	}
}

public class ChdescSetBlock extends Opcode
{
	public ChdescSetBlock(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescSetBlockFactory(input);
	}
}
