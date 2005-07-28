import java.io.DataInput;
import java.io.IOException;

public class ChdescSetBlock extends Opcode
{
	public ChdescSetBlock(int chdesc, int block)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_BLOCK, "KDB_CHDESC_SET_BLOCK", ChdescSetBlock.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("block", 4);
		return factory;
	}
}
