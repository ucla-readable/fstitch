import java.io.DataInput;
import java.io.IOException;

public class ChdescCreateBit extends Opcode
{
	public ChdescCreateBit(int chdesc, int block, int owner, short offset, int xor)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_BIT, "KDB_CHDESC_CREATE_BIT", ChdescCreateBit.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("owner", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("xor", 4);
		return factory;
	}
}
