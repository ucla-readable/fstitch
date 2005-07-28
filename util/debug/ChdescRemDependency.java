import java.io.DataInput;
import java.io.IOException;

class ChdescRemDependencyFactory extends ModuleOpcodeFactory
{
	public ChdescRemDependencyFactory(DataInput input)
	{
		super(input, KDB_CHDESC_REM_DEPENDENCY, "KDB_CHDESC_REM_DEPENDENCY");
		addParameter("source", 4);
		addParameter("target", 4);
	}
	
	public ChdescRemDependency readChdescRemDependency() throws UnexpectedOpcodeException, IOException
	{
		/* ... */
		return null;
	}
	
	public Opcode readOpcode() throws UnexpectedOpcodeException, IOException
	{
		return readChdescRemDependency();
	}
}

public class ChdescRemDependency extends Opcode
{
	public ChdescRemDependency(DataInput input)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		return new ChdescRemDependencyFactory(input);
	}
}
