import java.io.DataInput;
import java.io.IOException;

public class BdescDestroy extends Opcode
{
	public BdescDestroy(int block, int ddesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_DESTROY, "KDB_BDESC_DESTROY", BdescDestroy.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		return factory;
	}
}
