import java.io.DataInput;
import java.io.IOException;

class ChdescMergeFactory extends ModuleOpcodeFactory
{
	public ChdescMergeFactory(DataInput input)
	{
		super(input, KDB_CHDESC_MERGE, "KDB_CHDESC_MERGE");
		addParameter("count", 4);
		addParameter("chdescs", 4);
		addParameter("head", 4);
		addParameter("tail", 4);
	}
	
	public ChdescMerge readChdescMerge() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescMerge();
	}
}

public class ChdescMerge extends Opcode
{
	public ChdescMerge(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescMergeFactory(input);
	}
}
