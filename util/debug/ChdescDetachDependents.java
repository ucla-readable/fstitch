import java.io.DataInput;
import java.io.IOException;

class ChdescDetachDependentsFactory extends ModuleOpcodeFactory
{
	public ChdescDetachDependentsFactory(DataInput input)
	{
		super(input, KDB_CHDESC_DETACH_DEPENDENTS, "KDB_CHDESC_DETACH_DEPENDENTS");
		addParameter("chdesc", 4);
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
