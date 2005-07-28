import java.io.DataInput;
import java.io.IOException;

class ChdescAddDependencyFactory extends ModuleOpcodeFactory
{
	public ChdescAddDependencyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ADD_DEPENDENCY);
		addParameter("source", 4);
		addParameter("target", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_ADD_DEPENDENCY"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescAddDependency readChdescAddDependency() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescAddDependency();
	}
}

public class ChdescAddDependency extends Opcode
{
	public ChdescAddDependency(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescAddDependencyFactory(input);
	}
}
