import java.io.DataInput;
import java.io.IOException;

class ChdescRemDependentFactory extends ModuleOpcodeFactory
{
	public ChdescRemDependentFactory(DataInput input)
	{
		super(input, KDB_CHDESC_REM_DEPENDENT, "KDB_CHDESC_REM_DEPENDENT");
		addParameter("source", 4);
		addParameter("target", 4);
	}
	
	public ChdescRemDependent readChdescRemDependent() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescRemDependent();
	}
}

public class ChdescRemDependent extends Opcode
{
	public ChdescRemDependent(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescRemDependentFactory(input);
	}
}
