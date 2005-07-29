import java.io.DataInput;
import java.io.IOException;

public class ChdescAddDependency extends Opcode
{
	private final int source, target;
	
	public ChdescAddDependency(int source, int target)
	{
		this.source = source;
		this.target = target;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ADD_DEPENDENCY, "KDB_CHDESC_ADD_DEPENDENCY", ChdescAddDependency.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
