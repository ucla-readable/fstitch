import java.io.DataInput;
import java.io.IOException;

class ChdescApplyCollectionFactory extends ModuleOpcodeFactory
{
	public ChdescApplyCollectionFactory(DataInput input)
	{
		super(input, KDB_CHDESC_APPLY_COLLECTION, "KDB_CHDESC_APPLY_COLLECTION");
		addParameter("count", 4);
		addParameter("chdescs", 4);
		addParameter("order", 4);
	}
	
	public ChdescApplyCollection readChdescApplyCollection() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescApplyCollection();
	}
}

public class ChdescApplyCollection extends Opcode
{
	public ChdescApplyCollection(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescApplyCollectionFactory(input);
	}
}
