import java.io.DataInput;
import java.io.IOException;

public class BdescRetain extends Opcode
{
	public BdescRetain(int block, int ddesc, int ref_count, int ar_count, int dd_count)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_RETAIN, "KDB_BDESC_RETAIN", BdescRetain.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("ref_count", 4);
		factory.addParameter("ar_count", 4);
		factory.addParameter("dd_count", 4);
		return factory;
	}
}
