import java.io.DataInput;
import java.io.IOException;

public class ChdescSplit extends Opcode
{
	private final int original, count;
	
	public ChdescSplit(int original, int count)
	{
		this.original = original;
		this.count = count;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SPLIT, "KDB_CHDESC_SPLIT", ChdescSplit.class);
		factory.addParameter("original", 4);
		factory.addParameter("count", 4);
		return factory;
	}
}
