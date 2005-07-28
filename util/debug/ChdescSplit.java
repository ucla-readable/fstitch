import java.io.DataInput;
import java.io.IOException;

public class ChdescSplit extends Opcode
{
	public ChdescSplit(int original, int count)
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
