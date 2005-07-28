import java.io.DataInput;
import java.io.IOException;

public class ChdescMerge extends Opcode
{
	public ChdescMerge(int count, int chdescs, int head, int tail)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_MERGE, "KDB_CHDESC_MERGE", ChdescMerge.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("head", 4);
		factory.addParameter("tail", 4);
		return factory;
	}
}
