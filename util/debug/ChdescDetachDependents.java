import java.io.DataInput;
//import java.io.IOException;

public class ChdescDetachDependents extends Opcode
{
	private final int chdesc;
	
	public ChdescDetachDependents(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_DETACH_DEPENDENTS: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DETACH_DEPENDENTS, "KDB_CHDESC_DETACH_DEPENDENTS", ChdescDetachDependents.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
