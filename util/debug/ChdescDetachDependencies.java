import java.io.DataInput;
import java.io.IOException;

class ChdescDetachDependenciesFactory extends ModuleOpcodeFactory
{
	public ChdescDetachDependenciesFactory(DataInput input)
	{
		super(input, KDB_CHDESC_DETACH_DEPENDENCIES);
		addParameter("chdesc", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_DETACH_DEPENDENCIES"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescDetachDependencies readChdescDetachDependencies() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescDetachDependencies();
	}
}

public class ChdescDetachDependencies extends Opcode
{
	public ChdescDetachDependencies(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescDetachDependenciesFactory(input);
	}
}
