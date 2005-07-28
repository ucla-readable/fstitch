import java.io.DataInput;
import java.io.IOException;

class ChdescRollbackFactory extends ModuleOpcodeFactory
{
	public ChdescRollbackFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ROLLBACK);
		addParameter("chdesc", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_ROLLBACK"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescRollback readChdescRollback() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescRollback();
	}
}

public class ChdescRollback extends Opcode
{
	public ChdescRollback(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescRollbackFactory(input);
	}
}
