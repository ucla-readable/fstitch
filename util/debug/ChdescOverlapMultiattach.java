import java.io.DataInput;
import java.io.IOException;

public class ChdescOverlapMultiattach extends Opcode
{
	public ChdescOverlapMultiattach(int chdesc, int block, byte slip_under)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_OVERLAP_MULTIATTACH, "KDB_CHDESC_OVERLAP_MULTIATTACH", ChdescOverlapMultiattach.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("slip_under", 1);
		return factory;
	}
}
