import java.io.DataInput;
import java.io.IOException;

public class ChdescCreateNoop extends Opcode
{
	public ChdescCreateNoop(int chdesc, int block, int owner)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_NOOP, "KDB_CHDESC_CREATE_NOOP", ChdescCreateNoop.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("owner", 4);
		return factory;
	}
}
