import java.io.DataInput;
import java.io.IOException;

class ChdescRollbackCollectionFactory extends ModuleOpcodeFactory
{
	public ChdescRollbackCollectionFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ROLLBACK_COLLECTION);
		addParameter("count", 4);
		addParameter("chdescs", 4);
		addParameter("order", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_ROLLBACK_COLLECTION"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescRollbackCollection readChdescRollbackCollection() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescRollbackCollection();
	}
}

public class ChdescRollbackCollection extends Opcode
{
	public ChdescRollbackCollection(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescRollbackCollectionFactory(input);
	}
}
