import java.io.DataInput;
import java.io.IOException;

public class ChdescRemDependent extends Opcode
{
	private final int source, target;
	
	public ChdescRemDependent(int source, int target)
	{
		this.source = source;
		this.target = target;
	}
	
	public void applyTo(SystemState state)
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
