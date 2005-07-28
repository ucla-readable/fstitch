import java.io.DataInput;
import java.io.IOException;

public class ChdescCreateByte extends Opcode
{
	public ChdescCreateByte(int chdesc, int block, int owner, short offset, short length)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CREATE_BYTE, "KDB_CHDESC_CREATE_BYTE", ChdescCreateByte.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		factory.addParameter("owner", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("length", 2);
		return factory;
	}
}
