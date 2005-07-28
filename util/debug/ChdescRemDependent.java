import java.io.DataInput;
import java.io.IOException;

public class ChdescRemDependent extends Opcode
{
	public ChdescRemDependent(int source, int target)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_REM_DEPENDENT, "KDB_CHDESC_REM_DEPENDENT", ChdescRemDependent.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
