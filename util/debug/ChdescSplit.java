import java.io.DataInput;
import java.io.IOException;

class ChdescSplitFactory extends ModuleOpcodeFactory
{
	public ChdescSplitFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SPLIT);
		addParameter("original", 4);
		addParameter("count", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_SPLIT"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescSplit readChdescSplit() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescSplit();
	}
}

public class ChdescSplit extends Opcode
{
	public ChdescSplit(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescSplitFactory(input);
	}
}
