import java.io.DataInput;
import java.io.IOException;

public class ChdescDetachDependents extends Opcode
{
	public ChdescDetachDependents(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DETACH_DEPENDENTS, "KDB_CHDESC_DETACH_DEPENDENTS", ChdescDetachDependents.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
