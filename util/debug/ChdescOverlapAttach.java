import java.io.DataInput;
import java.io.IOException;

public class ChdescOverlapAttach extends Opcode
{
	public ChdescOverlapAttach(int recent, int original)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_OVERLAP_ATTACH, "KDB_CHDESC_OVERLAP_ATTACH", ChdescOverlapAttach.class);
		factory.addParameter("recent", 4);
		factory.addParameter("original", 4);
		return factory;
	}
}
