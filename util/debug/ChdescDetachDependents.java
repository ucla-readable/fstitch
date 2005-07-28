import java.io.DataInput;
import java.io.IOException;

class ChdescDetachDependentsFactory extends ModuleOpcodeFactory
{
	public ChdescDetachDependentsFactory(DataInput input)
	{
		super(input, KDB_CHDESC_DETACH_DEPENDENTS);
		addParameter("chdesc", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_DETACH_DEPENDENTS"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescDetachDependents readChdescDetachDependents() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescDetachDependents();
	}
}

public class ChdescDetachDependents extends Opcode
{
	public ChdescDetachDependents(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescDetachDependentsFactory(input);
	}
}
