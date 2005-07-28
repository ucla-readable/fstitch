import java.io.DataInput;
import java.io.IOException;

class ChdescAddDependentFactory extends ModuleOpcodeFactory
{
	public ChdescAddDependentFactory(DataInput input)
	{
		super(input, KDB_CHDESC_ADD_DEPENDENT);
		addParameter("source", 4);
		addParameter("target", 4);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals("KDB_CHDESC_ADD_DEPENDENT"))
			throw new UnexpectedNameException(name);
	}
	
	public ChdescAddDependent readChdescAddDependent() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescAddDependent();
	}
}

public class ChdescAddDependent extends Opcode
{
	public ChdescAddDependent(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescAddDependentFactory(input);
	}
}
