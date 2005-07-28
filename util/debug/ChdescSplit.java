import java.io.DataInput;
import java.io.IOException;

class ChdescSplitFactory extends ModuleOpcodeFactory
{
	public ChdescSplitFactory(DataInput input)
	{
		super(input, KDB_CHDESC_SPLIT, "KDB_CHDESC_SPLIT");
		addParameter("original", 4);
		addParameter("count", 4);
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
