import java.io.DataInput;
import java.io.IOException;

public class ChdescDetachDependencies extends Opcode
{
	public ChdescDetachDependencies(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DETACH_DEPENDENCIES, "KDB_CHDESC_DETACH_DEPENDENCIES", ChdescDetachDependencies.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
