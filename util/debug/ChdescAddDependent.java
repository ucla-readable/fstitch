import java.io.DataInput;
import java.io.IOException;

public class ChdescAddDependent extends Opcode
{
	public ChdescAddDependent(int source, int target)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ADD_DEPENDENT, "KDB_CHDESC_ADD_DEPENDENT", ChdescAddDependent.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
